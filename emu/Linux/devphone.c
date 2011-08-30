/* TODO/BUGS in no order:
   should send acknowledgement when an sms is read from /phone/sms, not just on every read
   decode_sms is sloppy--should convert septets into octets, then convert octets into UTF-8 instead of trying to do it all at once. split off into separate functions?
   handle sending/receiving multiple text messages and checking length of messages sent to /phone/sms
   consider notifying RIL of screen state using RIL_REQUEST_SCREEN_STATE to conserve power
   too many assumptions made in loop_for_data() about read() giving exactly the right amount of data
   parcel code will go crazy if ints overflow
   no way to give an error if SMSes don't send
   need to support DTMF
   get calling number when ring event occurs (through CALL_STATE_CHANGED?)
*/

#include "dat.h"
#include "fns.h"
#include "error.h"
#include "version.h"
#include "mp.h"
#include "libsec.h"
#include "keyboard.h"
#include <stdio.h>
#include <cutils/sockets.h>
#include <stdlib.h>
#include <telephony/ril.h>
#include <cutils/jstring.h>
#include <arpa/inet.h>
#include <libgen.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <semaphore.h>
#include <dlfcn.h>
#include "asound.h"
#include "alsa_audio.h"
#include "smsutils.h"
#include "parcel.h"
// tmp
#include "audit.h"
//end tmp

// RIL-related definitions

#define RESPONSE_SOLICITED 0
#define RESPONSE_UNSOLICITED 1
#define POWER_ON 2000
#define POWER_OFF 2001

typedef enum _SoundType {
    SOUND_TYPE_VOICE,
    SOUND_TYPE_SPEAKER,
    SOUND_TYPE_HEADSET,
    SOUND_TYPE_BTVOICE
} SoundType;

typedef enum _AudioPath {
    SOUND_AUDIO_PATH_HANDSET,
    SOUND_AUDIO_PATH_HEADSET,
    SOUND_AUDIO_PATH_SPEAKER,
    SOUND_AUDIO_PATH_BLUETOOTH,
    SOUND_AUDIO_PATH_BLUETOOTH_NO_NR,
    SOUND_AUDIO_PATH_HEADPHONE
} AudioPath;

// Inferno-related globals

enum
{
	Qdir,
	Qctl,
	Qsms,
	Qphone,
	Qsignal,
	Qstatus,
	Qcalls
};

Dirtab phonetab[] =
{
	".",	  {Qdir, 0, QTDIR}, 0, DMDIR|0555,
	"ctl",	  {Qctl},	    0, 0666,
	"sms",	  {Qsms},	    0, 0666,
	"phone",  {Qphone},	    0, 0666,
	"signal", {Qsignal},	    0, 0666,
	"status", {Qstatus},	    0, 0666,
	"calls",  {Qcalls},	    0, 0666,
};

Queue *phoneq;
Queue *smsq;

static int signal_strength;
static int fd;
static int power_state = 0;

static struct {
	QLock rl; // to prevent from waiting on rendez twice (causes a panic)
	Rendez r;
	int ready;
	int used;
	char *msg;
} status_msg;

static struct {
	QLock rl; // to prevent from waiting on rendez twice (causes a panic)
	Rendez r;
	int ready;
	int used;
	RIL_Call *cs;
	int num;
} calls;

void loop_for_data(void *v);
void send_sms(char *smsc_pdu, char *pdu);
void dial(char *number);
void activate_net(void);
void deactivate_net(void);
void radio_power(int power);

// AudioFlinger layer stuff
static void *af_handle;

void (*af_setMode)(int);
void (*af_setVoiceVolume)(float);
void (*af_setParameters)(char *);
void (*af_test)(void);

enum audio_mode {
        MODE_INVALID = -2,
        MODE_CURRENT = -1,
        MODE_NORMAL = 0,
        MODE_RINGTONE,
        MODE_IN_CALL,
        MODE_IN_COMMUNICATION,
        NUM_MODES  // not a valid entry, denotes end-of-list
};


int calls_ready(void *unused)
{
	return calls.ready;
}

int status_ready(void *unused)
{
	return status_msg.ready;
}

void phoneinit(void)
{
	af_handle = dlopen("libinfernoaudio.so", RTLD_NOW);
	if(af_handle == NULL) {
		fprintf(stderr, "opening libinfernoaudio.so failed: %s\n",
			dlerror());
	}
	af_setMode = dlsym(af_handle, "af_setMode");
	af_setVoiceVolume = dlsym(af_handle, "af_setVoiceVolume");
	af_setParameters = dlsym(af_handle, "af_setParameters");

	if(!(af_setMode && af_setVoiceVolume && af_setParameters)) {
		fprintf(stderr, "could not load AudioFlinger functions");
		dlclose(af_handle);
	}

	af_setMode(MODE_NORMAL);

	calls.cs = NULL;
	calls.num = 0;
	calls.ready = 0;
	calls.used = 0;
	
	status_msg.ready = 0;
	status_msg.used = 0;
	phoneq = qopen(512, 0, nil, nil);
	if(phoneq == 0)
		panic("no memory");
	smsq = qopen(512, 0, nil, nil);
	if(smsq == 0)
		panic("no memory");

	// set up proper permissions (need to be user radio, group radio to
	// access socket)
	if(setegid(1001) == -1) {
		perror("setegid(1001) failed");
	}
	if(seteuid(1001) == -1) {
		perror("seteuid(1001) failed");
	}

	fd = socket_local_client("rild",
				 ANDROID_SOCKET_NAMESPACE_RESERVED,
				 SOCK_STREAM);
	if(fd == -1) {
		perror("socket_local_client failed");
	}

	// set privileges back to root/root
	if(seteuid(0) == -1) {
		perror("seteuid(0) failed");
	}
	if(setegid(0) == -1) {
		perror("setegid(0) failed");
	}

	kproc("phone", loop_for_data, 0, 0);
}

static Chan *phoneattach(char *spec)
{
	// setup kprocs if necessary
	return devattach('f', spec);
}

static Walkqid *phonewalk(Chan *c, Chan *nc, char **name, int nname)
{
	return devwalk(c, nc, name, nname, phonetab, nelem(phonetab), devgen);
}

static int phonestat(Chan *c, uchar *db, int n)
{
	return devstat(c, db, n, phonetab, nelem(phonetab), devgen);
}

static Chan *phoneopen(Chan *c, int omode)
{
	c = devopen(c, omode, phonetab, nelem(phonetab), devgen);

	switch((ulong) c->qid.path) {
	case Qstatus:
		get_reg_state();
		break;
	case Qcalls:
		get_current_calls();
		break;
	}

	return c;
}

static void phoneclose(Chan *c)
{
	if((c->flag & COPEN) == 0)
		return;
}

static long phoneread(Chan *c, void *va, long n, vlong offset)
{
	char buf[255];
	int i, str_offset;
	if(c->qid.type & QTDIR) {
		return devdirread(c, va, n, phonetab, nelem(phonetab), devgen);
	}

	switch((ulong) c->qid.path) {
	case Qctl:
		return readstr(offset, va, n, power_state ? "on\n" : "off\n");
	case Qphone:
		return qread(phoneq, va, n);
	case Qsms:
		// FIXME only acknowledge when an SMS is being read
		acknowledge_sms();
		return qread(smsq, va, n);
	case Qsignal:
		snprintf(buf, sizeof(buf), "%d\n", signal_strength);
		return readstr(offset, va, n, buf);
	case Qstatus:
		Sleep(&status_msg.r, status_ready, NULL);
		if(status_msg.ready == -1) {
			// an error occurred
			status_msg.used = 1;
			return readstr(offset, va, n, "error\n");
		}
		if(status_msg.used) {
			status_msg.ready = 0;
			status_msg.used = 0;
			return 0; // already sent the data
		}
		status_msg.used = 1;
		return readstr(offset, va, n, status_msg.msg);
	case Qcalls:
		Sleep(&calls.r, calls_ready, NULL);
		if(calls.ready == -1) {
			// an error occurred
			calls.used = 1;
			return readstr(offset, va, n, "error\n");
		}
		if(calls.used) {
			calls.ready = 0;
			calls.used = 0;
			return 0; // already sent the data
		}
		calls.used = 1;

		str_offset = 0;
		for(i = 0; i < calls.num; i++) {
			RIL_Call c = calls.cs[i];
			str_offset += snprintf(va + str_offset, n - str_offset,
					       "call %d:\n "
					       " state %d\n index %d\n toa %d\n"
					       " isMpty %d\n isMT %d\n als %d\n isVoice %d\n"
					       " isVoicePrivacy %d\n number %s\n"
					       " numberPresentation %d\n name %s\n"
					       " namePresentation %d\n",
					       i, c.state, c.index, c.toa, c.isMpty, c.isMT,
					       c.als, c.isVoice, c.isVoicePrivacy, c.number,
					       c.numberPresentation, c.name,
					       c.namePresentation);
		}
		return str_offset;
	}
	return 0;
}

static long phonewrite(Chan *c, void *va, long n, vlong offset)
{
	char *pdu;
	char *args[5];
	char *str;
	Rune *runestr;
	int nargs = 0, i, j;

	if(c->qid.type & QTDIR)
		error(Eperm);
	switch((ulong) c->qid.path) {
	case Qctl:
		str = auditmalloc((n + 1) * sizeof(char));
		strncpy(str, va, n);
		str[n] = '\0';
		printf("Qctl: va = %s\n", str);
		nargs = getfields(str, args, sizeof(args), 1, " \n");
		if(strcmp(args[0], "on") == 0) {
			radio_power(1);
		} else if(strcmp(args[0], "off") == 0) {
			radio_power(0);
		} else if(strcmp(args[0], "net") == 0) {
			if(nargs >= 2) {
				if(strcmp(args[1], "on") == 0) {
					activate_net();
				} else if(strcmp(args[1], "off") == 0) {
					deactivate_net();
				}
			}
		} else if(strcmp(args[0], "mute") == 0) {
			set_mute(1);
		} else if(strcmp(args[1], "unmute") == 0) {
			set_mute(0);
		}
		auditfree(str);
		break;
	case Qsms:
		str = auditmalloc((n + 1) * sizeof(char));
		strncpy(str, va, n);
		str[n] = '\0';
		printf("Qsms: va = %s\n", str);
		nargs = getfields(str, args, 3, 1, " ");
		if(strcmp(args[0], "send") == 0) {
			if(nargs == 3) {
				runestr = auditmalloc((utflen(args[2]) + 1) * sizeof(Rune));
				for(i = 0, j = 0; j < strlen(args[2]); i++) {
					j += chartorune(runestr + i, args[2] + j);
				}
				runestr[i] = 0;
				pdu = encode_sms(args[1], runestr);
				send_sms(NULL, pdu);
				
				auditfree(runestr);
				auditfree(pdu);
			}
		}
		auditfree(str);
		break;
	case Qphone:
		str = auditmalloc((n + 1) * sizeof(char));
		strncpy(str, va, n);
		str[n] = '\0';
		printf("Qphone: va = %s\n", str);
		nargs = getfields(str, args, sizeof(args), 1, " \n");
		if(strcmp(args[0], "answer") == 0) {
			answer();
		} else if(strcmp(args[0], "dial") == 0) {
			if(nargs != 2) {
				fprintf(stderr, "malformed dial request\n");
				auditfree(str);
				break;
			}
			dial(args[1]);
		} else if(strcmp(args[0], "hangup") == 0) {
			if(nargs != 2) {
				fprintf(stderr, "malformed hangup request\n");
				auditfree(str);
				break;
			}
			hangup(atoi(args[1]));
		}
		auditfree(str);
		break;
	}
	return n;
}

Dev phonedevtab = {
	'f',
	"phone",

	phoneinit,
	phoneattach,
	phonewalk,
	phonestat,
	phoneopen,
	devcreate,
	phoneclose,
	phoneread,
	devbread,
	phonewrite,
	devbwrite,
	devremove,
	devwstat
};

/* RIL functions */

void handle_error(int seq, int error)
{
	// RIL error messages
	char *errmsgs[] = { "no error", "radio not available", "generic failure", "password incorrect", "need SIM PIN2", "need SIM PUK2", "request not supported", "cancelled", "cannot access network during voice calls", "cannot access network before registering to network", "retry sending sms", "no SIM", "no subscription", "mode not supported", "FDN list check failed", "illegal SIM or ME" };
	char *errmsg = errmsgs[error];
	switch(seq) {
	case RIL_REQUEST_SEND_SMS:
		qproduce(smsq, errmsg, strlen(errmsg));
		break;
	case RIL_REQUEST_DIAL:
		qproduce(phoneq, errmsg, strlen(errmsg));
		break;
	case RIL_REQUEST_REGISTRATION_STATE:
		status_msg.ready = -1;
		Wakeup(&status_msg.r);
		break;
	case RIL_REQUEST_GET_CURRENT_CALLS:
		calls.ready = -1;
		Wakeup(&calls.r);
		break;
	}
}

void handle_sol_response(struct parcel *p)
{
	int seq, error, i, num, offset = 0;
	char buf[200];
	seq = parcel_r_int32(p);
	error = parcel_r_int32(p);
	printf("received sol response: seq: %d err: %d\n", seq, error);
	if(error != 0) {
		handle_error(seq, error);
		return;
	}
	switch(seq) {
	case RIL_REQUEST_SETUP_DATA_CALL:
		if(!parcel_data_avail(p)) return;
		num = parcel_r_int32(p);
		for(i = 0; i < num; i++) {
			printf("%s\n", parcel_r_string(p));
		}
		break;
	case POWER_ON:
		power_state = 1;
		break;
	case POWER_OFF:
		power_state = 0;
		af_setMode(MODE_NORMAL);
		break;
	case RIL_REQUEST_REGISTRATION_STATE:
		if(!parcel_data_avail(p)) return;
		num = parcel_r_int32(p);
		offset = 0;
		for(i = 0; i < num; i++) {
			offset += snprintf(buf + offset, sizeof(buf) - offset, "%s\n", parcel_r_string(p));
		}
		auditfree(status_msg.msg);
		status_msg.msg = auditstrdup(buf);
		status_msg.ready = 1;
		Wakeup(&status_msg.r);
		break;
	case RIL_REQUEST_DIAL:
		printf("got RIL_REQUEST_DIAL success\n");
		break;
	case RIL_REQUEST_GET_CURRENT_CALLS:
		if(!parcel_data_avail(p)) {
			calls.ready = 1;
			calls.num = 0;
			Wakeup(&calls.r);
			break;
		}

		for(i = 0; i < calls.num; i++) {
			auditfree(calls.cs[i].number);
			auditfree(calls.cs[i].name);
			auditfree(calls.cs[i].uusInfo);
		}
		auditfree(calls.cs);
		num = parcel_r_int32(p);
		calls.num = num;
		calls.cs = auditmalloc(num * sizeof(RIL_Call));
		if(calls.cs == NULL) {
			fprintf(stderr, "malloc failed while making %d RIL_Calls\n", num);
			calls.ready = -1;
			Wakeup(&calls.r);
			break;
		}

		for(i = 0; i < num; i++) {
			calls.cs[i].state = parcel_r_int32(p);
			calls.cs[i].index = parcel_r_int32(p);
			calls.cs[i].toa = parcel_r_int32(p);
			calls.cs[i].isMpty = parcel_r_int32(p);
			calls.cs[i].isMT = parcel_r_int32(p);
			calls.cs[i].als = parcel_r_int32(p);
			calls.cs[i].isVoice = parcel_r_int32(p);
			calls.cs[i].isVoicePrivacy = parcel_r_int32(p);
			calls.cs[i].number = parcel_r_string(p);
			calls.cs[i].numberPresentation = parcel_r_int32(p);
			calls.cs[i].name = parcel_r_string(p);
			calls.cs[i].namePresentation = parcel_r_int32(p);
			calls.cs[i].uusInfo = parcel_r_int32(p) ?
					      auditmalloc(sizeof(RIL_UUS_Info)) :
					      NULL;
			if(calls.cs[i].uusInfo != NULL) {
				// FIXME not implemented yet
				printf("debug: got uusInfo\n");
			}
		}
		calls.ready = 1;
		Wakeup(&calls.r);
		break;
	}
}

void handle_unsol_response(struct parcel *p)
{
	int resp_type;
	char buf[300];
	int l;
	struct recvd_sms sms;

	resp_type = parcel_r_int32(p);
//	printf("unsolicited: type = %d, data left = %d\n", resp_type,
//	       parcel_data_avail(p));
	switch(resp_type) {
	case RIL_UNSOL_RESPONSE_NEW_SMS:
		decode_sms(parcel_r_string(p), &sms);
		l = snprintf(buf, sizeof(buf), "%s (%s): %s\n", sms.src_num,
			     sms.timestamp, sms.msg);
		buf[l] = '\0';
		qproduce(smsq, buf, strlen(buf));

		auditfree(sms.msg);
		auditfree(sms.service_center);
		auditfree(sms.src_num);
		auditfree(sms.timestamp);
		break;
	case RIL_UNSOL_CALL_RING:
		printf("incoming call\n");
		qproduce(phoneq, "ring\n", 5);
		break;
	case RIL_UNSOL_SIGNAL_STRENGTH:
		signal_strength = parcel_r_int32(p);
		// TODO: use bit error rate instead of cell tower reception
		// when available (during a call)
		break;
	}
}

// FIXME: read() needs to be handled better in this function, allow it to
// underrun and just loop again to get the rest of the data
void loop_for_data(void *v)
{
	seteuid(1001);
	for(;;) {
		int msglen, readlen, type, i, ret;
		char *buf;
		struct parcel p;

		parcel_init(&p);
		if((ret = read(fd, &msglen, sizeof(msglen))) != sizeof(msglen)) {
			perror("loop_for_data");
			fprintf(stderr, "EOF while reading parcel length\n");
			fprintf(stderr, "read returned %d\n", ret); 
			return;
		}
		msglen = ntohl(msglen);
		buf = (char *) auditmalloc(msglen * sizeof(char));
		if(buf == NULL) {
			fprintf(stderr, "devphone:loop_for_data: malloc failed when attempting to allocate %d bytes\n", msglen);
			return;
		}
		readlen = read(fd, buf, msglen);
		if(readlen != msglen) {
			fprintf(stderr, "msglen = %d readlen = %d\n", msglen,
				readlen);
			fprintf(stderr,
				"got too much or not enough data, aborting\n");
			auditfree(buf);
			return;
		}
		/*	for(i = 0; i < readlen; i++) {
			printf("%02x", buf[i]);
		}
		printf("\n");*/
		parcel_grow(&p, msglen);
		memcpy(p.data, buf, msglen);
		p.size = msglen;
		//	printf("Received %d bytes of data from rild.\n", msglen);
		type = parcel_r_int32(&p);
		if(type == RESPONSE_SOLICITED) {
			handle_sol_response(&p);
		} else {
			handle_unsol_response(&p);
		}
		parcel_free(&p);
		auditfree(buf);
	}
}

void send_ril_parcel(struct parcel *p)
{
	int pktlen, ret;

	// we write the length in bytes, then the parcel data
	pktlen = htonl(p->size);
	ret = write(fd, (void *)&pktlen, sizeof(pktlen));
	if(ret < 0) perror("write to ril failed");
	ret = write(fd, p->data, p->size);
	if(ret < 0) perror("write to ril failed");
}

void answer(void)
{
	struct parcel p;
	
	parcel_init(&p);
	parcel_w_int32(&p, RIL_REQUEST_ANSWER); // request id
	parcel_w_int32(&p, RIL_REQUEST_ANSWER); // reply id

	send_ril_parcel(&p);
	parcel_free(&p);
}

void radio_power(int power)
{
	struct parcel p;

	parcel_init(&p);
	parcel_w_int32(&p, RIL_REQUEST_RADIO_POWER); // request id
	parcel_w_int32(&p, power ? POWER_ON : POWER_OFF); // reply id
	parcel_w_int32(&p, 1); // ??? java code does it
	parcel_w_int32(&p, power); // 1 on, 0 off

	send_ril_parcel(&p);
	parcel_free(&p);
}

void dial(char *number)
{
	struct parcel p;
	struct mixer *mixer;
	struct mixer_ctl *ctl;

	af_setMode(MODE_IN_CALL); // tell audioflinger we want to start a call
	af_setParameters("routing=1"); // tell AF where to route sound
	af_setVoiceVolume(1.0f);
	set_mute(0);

	parcel_init(&p);
	parcel_w_int32(&p, RIL_REQUEST_DIAL); // request id
	parcel_w_int32(&p, RIL_REQUEST_DIAL); // reply id
	parcel_w_string(&p, number); // number to dial
	parcel_w_int32(&p, 0); // clir mode
	parcel_w_int32(&p, 0); // 0 - uus info absent, 1 - uus info present
	parcel_w_int32(&p, 0); // 0 - uus info absent, 1 - uus info present
	                       // yes, twice...??

	send_ril_parcel(&p);
	parcel_free(&p);
}

void activate_net(void)
{
	struct parcel p;

	parcel_init(&p);
	parcel_w_int32(&p, RIL_REQUEST_SETUP_DATA_CALL); // request ID
	parcel_w_int32(&p, RIL_REQUEST_SETUP_DATA_CALL); // reply id
	parcel_w_int32(&p, 7); // undocumented magic.
	parcel_w_string(&p, "1"); // CDMA or GSM. 1 = GSM
	parcel_w_string(&p, "0"); // data profile. 0 = default
	parcel_w_string(&p, "wap.cingular"); // APN name
	parcel_w_string(&p, "WAP@CINGULARGPRS.COM"); // APN user
	parcel_w_string(&p, "cingular1"); // APN passwd
	parcel_w_string(&p, "0"); // auth type. 0 usually

	send_ril_parcel(&p);
	parcel_free(&p);
}

void deactivate_net(void)
{
	struct parcel p;
	
	parcel_init(&p);
	parcel_w_int32(&p, RIL_REQUEST_DEACTIVATE_DATA_CALL); // request id
	parcel_w_int32(&p, RIL_REQUEST_DEACTIVATE_DATA_CALL); // reply id
	parcel_w_string(&p, "1"); // FIXME: this should be the CID returned by
				  // RIL_REQUEST_SETUP_DATA_CALL response
	send_ril_parcel(&p);
	parcel_free(&p);
}

void send_sms(char *smsc_pdu, char *pdu)
{
	struct parcel p;

	parcel_init(&p);
	parcel_w_int32(&p, RIL_REQUEST_SEND_SMS); // request id
	parcel_w_int32(&p, RIL_REQUEST_SEND_SMS); // reply id
	parcel_w_int32(&p, 2); // ??? java code does it
	parcel_w_string(&p, smsc_pdu); // SMSC PDU
	parcel_w_string(&p, pdu); // SMS PDU

	send_ril_parcel(&p);
	parcel_free(&p);
}

void acknowledge_sms(void)
{
	struct parcel p;

	parcel_init(&p);
	parcel_w_int32(&p, RIL_REQUEST_SMS_ACKNOWLEDGE); // request id
	parcel_w_int32(&p, RIL_REQUEST_SMS_ACKNOWLEDGE); // reply id
	parcel_w_int32(&p, 2); // ??? java code does it
	parcel_w_int32(&p, 1); // success
	parcel_w_int32(&p, 0); // failure cause if necessary

	send_ril_parcel(&p);
	parcel_free(&p);
}

void get_reg_state(void)
{
	struct parcel p;
	
	parcel_init(&p);
	parcel_w_int32(&p, RIL_REQUEST_REGISTRATION_STATE); // request id
	parcel_w_int32(&p, RIL_REQUEST_REGISTRATION_STATE); // reply id
	
	send_ril_parcel(&p);
	parcel_free(&p);
}

void set_mute(int muted)
{
	struct parcel p;
	
	parcel_init(&p);
	parcel_w_int32(&p, RIL_REQUEST_SET_MUTE); // request id
	parcel_w_int32(&p, RIL_REQUEST_SET_MUTE); // reply id
	parcel_w_int32(&p, 1); // ??? java code does this
	parcel_w_int32(&p, muted); // 0 - unmute, 1 - mute

	send_ril_parcel(&p);
	parcel_free(&p);
}

void get_current_calls(void)
{
	struct parcel p;

	parcel_init(&p);
	parcel_w_int32(&p, RIL_REQUEST_GET_CURRENT_CALLS); // request id
	parcel_w_int32(&p, RIL_REQUEST_GET_CURRENT_CALLS); // reply id

	send_ril_parcel(&p);
	parcel_free(&p);
}

void hangup(int index)
{
	struct parcel p;
	
	parcel_init(&p);
	parcel_w_int32(&p, RIL_REQUEST_HANGUP); // request id
	parcel_w_int32(&p, RIL_REQUEST_HANGUP); // reply id
	parcel_w_int32(&p, 1); // ??? java code does it, don't know why
	parcel_w_int32(&p, index); // line index to hang up

	send_ril_parcel(&p);
	parcel_free(&p);
}
